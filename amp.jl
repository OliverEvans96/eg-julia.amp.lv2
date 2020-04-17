module julia_amp

function db_to_coef(gain)
    coef = gain < 9.0f0 ? exp10(-0.05f0 * gain) : 0.0f0
    return coef
end

end
